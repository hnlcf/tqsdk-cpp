#!/usr/bin/env python3
"""P0 SimNow acceptance: advanced orders, risk rules, target tasks.

Read-only by default. Write scenarios require both --write and --yes.

Examples:

    python3 scripts/p0_simnow_acceptance.py \
        --node-url http://127.0.0.1:18081 \
        --symbol SHFE.rb2701

    python3 scripts/p0_simnow_acceptance.py \
        --node-url http://127.0.0.1:18081 \
        --symbol SHFE.rb2701 --write --yes

    # Also exercise TargetPosTask(1) and clean it up:
    python3 scripts/p0_simnow_acceptance.py ... --write --yes --target-write

The script never reads credentials and never starts/restarts node/gateway.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

PASS, FAIL, WARN, SKIP = "PASS", "FAIL", "WARN", "SKIP"


@dataclass
class Check:
    name: str
    status: str
    detail: str = ""
    evidence: Any = None


@dataclass
class Report:
    node_url: str
    symbol: str
    started_at: str
    checks: list[Check] = field(default_factory=list)
    finished_at: str = ""

    def add(self, name: str, status: str, detail: str = "", evidence: Any = None) -> None:
        self.checks.append(Check(name, status, detail, evidence))
        print(f"[{status}] {name}: {detail}")

    def counts(self) -> dict[str, int]:
        out = {PASS: 0, FAIL: 0, WARN: 0, SKIP: 0}
        for check in self.checks:
            out[check.status] = out.get(check.status, 0) + 1
        return out

    def exit_code(self) -> int:
        return 1 if self.counts()[FAIL] else 0

    def to_dict(self) -> dict[str, Any]:
        return {
            "node_url": self.node_url,
            "symbol": self.symbol,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "summary": self.counts(),
            "checks": [asdict(c) for c in self.checks],
        }


class Http:
    def __init__(self, base: str, timeout: float) -> None:
        self.base = base.rstrip("/")
        self.timeout = timeout

    def request(self, method: str, path: str, body: Any = None) -> tuple[int, Any, str]:
        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read()
                try:
                    parsed = json.loads(raw) if raw else None
                except json.JSONDecodeError:
                    parsed = raw.decode(errors="replace")
                return resp.status, parsed, ""
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            try:
                parsed = json.loads(raw) if raw else None
            except json.JSONDecodeError:
                parsed = raw.decode(errors="replace")
            return exc.code, parsed, f"HTTP {exc.code}"
        except Exception as exc:  # noqa: BLE001
            return 0, None, str(exc)

    def get(self, path: str) -> tuple[int, Any, str]:
        return self.request("GET", path)

    def post(self, path: str, body: dict[str, Any]) -> tuple[int, Any, str]:
        return self.request("POST", path, body)

    def delete(self, path: str) -> tuple[int, Any, str]:
        return self.request("DELETE", path)


def finite(value: Any) -> float | None:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return None
    return out if math.isfinite(out) else None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class Acceptance:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.http = Http(args.node_url, args.timeout)
        self.report = Report(args.node_url, args.symbol, utc_now())
        self.quote: dict[str, Any] = {}
        self.order_ids: list[str] = []
        self.task_ids: list[str] = []

    def check(self, name: str, status: str, detail: str = "", evidence: Any = None) -> None:
        self.report.add(name, status, detail, evidence)

    def run(self) -> int:
        try:
            self.check_health()
            ready = self.check_ready()
            self.check_catalog()
            self.check_quote()
            if ready and self.args.write:
                if not self.args.yes:
                    self.check("write", FAIL, "写场景需要显式 --yes")
                else:
                    self.cleanup_all()
                    self.check_advanced_orders()
                    self.check_risk_rule()
                    self.check_target_task()
            elif ready:
                self.check("write", SKIP, "未指定 --write，跳过写场景")
            self.check_tasks()
        finally:
            self.cleanup_all()
            self.report.finished_at = utc_now()
            self.write_report()
        return self.report.exit_code()

    def check_health(self) -> None:
        status, body, error = self.http.get("/healthz")
        ok = status == 200 and isinstance(body, dict) and body.get("ok")
        self.check("healthz", PASS if ok else FAIL, f"HTTP {status} {error}", body)

    def check_ready(self) -> bool:
        deadline = time.monotonic() + self.args.ready_timeout
        last: Any = None
        while time.monotonic() < deadline:
            status, body, _ = self.http.get("/readyz")
            last = body
            if status == 200 and isinstance(body, dict) and body.get("ready"):
                self.check("readyz", PASS, "ready=true", body)
                return True
            time.sleep(0.5)
        self.check("readyz", FAIL, "等待 readyz 超时", last)
        return False

    def check_catalog(self) -> None:
        status, body, _ = self.http.get("/catalog/status")
        ok = status == 200 and isinstance(body, dict) and body.get("loaded") is True
        self.check("catalog", PASS if ok else WARN, f"HTTP {status}", body)

    def check_quote(self) -> None:
        deadline = time.monotonic() + self.args.quote_timeout
        last: Any = None
        while time.monotonic() < deadline:
            status, body, _ = self.http.get(f"/quote/{self.args.symbol}")
            last = body
            if status == 200 and isinstance(body, dict):
                if finite(body.get("last_price")) is not None:
                    self.quote = body
                    self.check("quote", PASS, f"last={body.get('last_price')}", body)
                    return
            time.sleep(0.5)
        self.check("quote", FAIL, "没有有限 last_price", last)

    def task_lines(self) -> list[str]:
        _status, body, _ = self.http.get("/tasks")
        lines: list[str] = []
        if isinstance(body, dict):
            for key in ("targets", "orders"):
                items = body.get(key)
                if isinstance(items, list):
                    lines.extend(str(x.get("task_id", "")) for x in items if isinstance(x, dict))
        return lines

    def cleanup_all(self) -> None:
        for task_id in list(self.task_ids):
            self.http.delete(f"/task/target/{task_id}")
            self.http.delete(f"/task/order/{task_id}")
        self.task_ids.clear()
        for order_id in list(self.order_ids):
            self.http.delete(f"/order/{order_id}")
        self.order_ids.clear()
        self.http.delete("/risk/rules")

    def safe_limit(self, direction: str = "BUY") -> float | None:
        tick = finite(self.quote.get("price_tick")) or 1.0
        bid = finite(self.quote.get("bid_price1"))
        ask = finite(self.quote.get("ask_price1"))
        if direction == "BUY":
            ref = bid if bid is not None else ask
            if ref is None:
                return None
            return math.floor((ref - 3.0 * tick) / tick + 1e-9) * tick
        ref = ask if ask is not None else bid
        if ref is None:
            return None
        return math.ceil((ref + 3.0 * tick) / tick - 1e-9) * tick

    def order_payload(self, advanced: str) -> dict[str, Any] | None:
        price = self.safe_limit("BUY")
        if price is None or price <= 0:
            return None
        return {
            "instrument_id": self.args.symbol,
            "direction": "BUY",
            "offset": "OPEN",
            "price_type": "LIMIT",
            "limit_price": price,
            "time_cond": "GFD" if advanced == "NONE" else "IOC",
            "volume_cond": "ALL" if advanced == "FOK" else "ANY",
            "advanced": advanced,
            "volume": 1,
        }

    def submit(self, payload: dict[str, Any], scenario: str) -> tuple[bool, str, Any]:
        status, body, error = self.http.post("/order", payload)
        evidence = {"request": payload, "http_status": status, "response": body, "error": error}
        if status != 200 or not isinstance(body, dict):
            self.check(scenario, FAIL, f"HTTP {status}: {error}", evidence)
            return False, "", evidence
        order_id = str(body.get("order_id") or "")
        if order_id:
            self.order_ids.append(order_id)
        evidence["order_id"] = order_id
        return bool(body.get("success")), order_id, evidence

    def wait_order(self, order_id: str) -> dict[str, Any] | None:
        deadline = time.monotonic() + self.args.order_timeout
        last = None
        while time.monotonic() < deadline:
            status, body, _ = self.http.get("/orders")
            if status == 200 and isinstance(body, dict):
                orders = body.get("orders")
                if isinstance(orders, dict):
                    last = orders.get(order_id)
                    if isinstance(last, dict) and (last.get("is_dead") or last.get("is_error")):
                        return last
            time.sleep(0.25)
        return last if isinstance(last, dict) else None

    def cancel(self, order_id: str) -> None:
        if not order_id:
            return
        self.http.delete(f"/order/{order_id}")

    def check_advanced_orders(self) -> None:
        for advanced in ("FAK", "FOK"):
            payload = self.order_payload(advanced)
            if payload is None:
                self.check(f"advanced.{advanced.lower()}", SKIP, "无有效 quote")
                continue
            ok, order_id, evidence = self.submit(payload, f"advanced.{advanced.lower()}.submit")
            if not ok:
                self.check(f"advanced.{advanced.lower()}", FAIL, "订单未被受理", evidence)
                continue
            order = self.wait_order(order_id)
            evidence["order"] = order
            # FAK/FOK 的远端语义允许立即终态；活着则撤掉，避免遗留。
            self.cancel(order_id)
            self.check(
                f"advanced.{advanced.lower()}",
                PASS if order is not None else WARN,
                f"order_id={order_id} status={order.get('status') if order else 'missing'}",
                evidence,
            )

        if self.args.five_level:
            if not self.args.symbol.startswith("CFFEX."):
                self.check("advanced.fivelevel", SKIP, "FIVELEVEL 仅 CFFEX 场景启用")
            else:
                payload = self.order_payload("NONE")
                if payload:
                    payload["price_type"] = "FIVELEVEL"
                    payload["limit_price"] = 0
                    ok, order_id, evidence = self.submit(payload, "advanced.fivelevel.submit")
                    self.cancel(order_id)
                    self.check("advanced.fivelevel", PASS if ok else FAIL, "", evidence)

    def check_risk_rule(self) -> None:
        self.http.delete("/risk/rules")
        status, body, error = self.http.post(
            "/risk/rules",
            {"type": "OPEN_COUNT_LIMIT", "limit": 1, "symbols": [self.args.symbol]},
        )
        if status != 200:
            self.check("risk.configure", FAIL, f"HTTP {status}: {error}", body)
            return
        first = self.order_payload("NONE")
        if first is None:
            self.check("risk.open_count_limit", SKIP, "无有效 quote")
            return
        ok, first_id, evidence = self.submit(first, "risk.first_order")
        if not ok:
            self.check("risk.open_count_limit", WARN, "首单未受理，风控场景无法继续", evidence)
            return
        self.wait_order(first_id)
        second = dict(first)
        ok2, second_id, evidence2 = self.submit(second, "risk.second_order")
        if second_id:
            self.order_ids.append(second_id)
        self.cancel(first_id)
        self.http.delete("/risk/rules")
        rejected = not ok2 and isinstance(evidence2.get("response"), dict) and "风控" in str(
            evidence2["response"].get("error", "")
        )
        self.check(
            "risk.open_count_limit",
            PASS if rejected else FAIL,
            "第二单被风控拒绝" if rejected else "第二单未被风控拒绝",
            {"first": evidence, "second": evidence2},
        )

    def check_target_task(self) -> None:
        # 安全场景：target=0 只验证任务生命周期，不会发单。
        payload = {"symbol": self.args.symbol, "target_volume": 0, "mode": "ACTIVE"}
        status, body, error = self.http.post("/task/target", payload)
        if status != 200 or not isinstance(body, dict) or not body.get("success"):
            self.check("task.target.create", FAIL, f"HTTP {status}: {error}", body)
            return
        task_id = str(body.get("task_id") or "")
        self.task_ids.append(task_id)
        self.check("task.target.create", PASS, f"task_id={task_id}", body)
        _status, tasks, _ = self.http.get("/tasks")
        self.check(
            "task.target.list",
            PASS if isinstance(tasks, dict) and any(
                isinstance(x, dict) and x.get("task_id") == task_id
                for x in tasks.get("targets", [])
            ) else FAIL,
            "任务出现在 /tasks",
            tasks,
        )
        self.http.delete(f"/task/target/{task_id}")
        self.task_ids.remove(task_id)

        if not self.args.target_write:
            self.check("task.target.write", SKIP, "未指定 --target-write")
            return

        # 真实写场景：小目标 1 手，随后撤任务并由脚本清理。
        status, body, error = self.http.post(
            "/task/target",
            {"symbol": self.args.symbol, "target_volume": 1, "mode": "ACTIVE"},
        )
        if status != 200 or not isinstance(body, dict) or not body.get("success"):
            self.check("task.target.write", FAIL, f"HTTP {status}: {error}", body)
            return
        task_id = str(body.get("task_id") or "")
        self.task_ids.append(task_id)
        deadline = time.monotonic() + self.args.order_timeout
        last_tasks: Any = None
        last_positions: Any = None
        target_reached = False
        while time.monotonic() < deadline:
            _status, last_tasks, _ = self.http.get("/tasks")
            _status, last_positions, _ = self.http.get("/positions")
            if isinstance(last_positions, dict):
                positions = last_positions.get("positions", {})
                row = positions.get(self.args.symbol) if isinstance(positions, dict) else None
                if isinstance(row, dict):
                    net = int(row.get("volume_long", 0)) - int(row.get("volume_short", 0))
                    if net >= 1:
                        target_reached = True
                        break
            time.sleep(0.25)
        self.check(
            "task.target.write",
            PASS if target_reached else WARN,
            "目标净持仓已达到 1 手" if target_reached
            else "TargetPosTask 已运行，但未在超时内确认目标仓位；请人工确认",
            {"tasks": last_tasks, "positions": last_positions},
        )
        self.http.delete(f"/task/target/{task_id}")
        self.task_ids.remove(task_id)

    def check_tasks(self) -> None:
        status, body, _ = self.http.get("/tasks")
        ok = status == 200 and isinstance(body, dict)
        self.check("tasks", PASS if ok else FAIL, f"HTTP {status}", body)

    def write_report(self) -> None:
        out_dir = Path(self.args.out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        base = out_dir / f"p0_simnow_{stamp}"
        data = self.report.to_dict()
        base.with_suffix(".json").write_text(json.dumps(data, ensure_ascii=False, indent=2))
        counts = self.report.counts()
        lines = [
            "# P0 SimNow acceptance",
            "",
            f"- Node: `{self.report.node_url}`",
            f"- Symbol: `{self.report.symbol}`",
            f"- Summary: PASS={counts[PASS]} FAIL={counts[FAIL]} WARN={counts[WARN]} SKIP={counts[SKIP]}",
            "",
            "| Status | Check | Detail |",
            "|---|---|---|",
        ]
        for c in self.report.checks:
            lines.append(f"| {c.status} | `{c.name}` | {c.detail.replace('|', '\\|')} |")
        base.with_suffix(".md").write_text("\n".join(lines) + "\n")
        print(f"report: {base.with_suffix('.md')}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--symbol", default="SHFE.rb2701")
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--ready-timeout", type=float, default=20.0)
    parser.add_argument("--quote-timeout", type=float, default=20.0)
    parser.add_argument("--order-timeout", type=float, default=10.0)
    parser.add_argument("--out-dir", default="artifacts")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--yes", action="store_true")
    parser.add_argument("--five-level", action="store_true")
    parser.add_argument("--target-write", action="store_true")
    return Acceptance(parser.parse_args()).run()


if __name__ == "__main__":
    sys.exit(main())
