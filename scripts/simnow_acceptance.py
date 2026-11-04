#!/usr/bin/env python3
"""SimNow/CTP acceptance probe for tqsdk-node.

The script deliberately treats order placement as opt-in:

    python3 scripts/simnow_acceptance.py                 # read-only checks
    python3 scripts/simnow_acceptance.py --orders --yes  # write checks

It does not start tqsdk-gw/node. Start them first, for example:

    ./scripts/run_live.sh --no-ui
    python3 scripts/simnow_acceptance.py \
        --node-url http://127.0.0.1:18081 \
        --symbol SHFE.rb2701 --orders --yes

Order checks use a far-from-market buy limit price and cancel it immediately.
The optional market-order check is never enabled by --orders; it requires
--include-market and is intentionally left off by default.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    import websockets  # type: ignore
except Exception:  # pragma: no cover - optional dependency
    websockets = None


PASS = "PASS"
FAIL = "FAIL"
WARN = "WARN"
SKIP = "SKIP"


@dataclass
class CheckResult:
    name: str
    status: str
    detail: str = ""
    evidence: Any = None


@dataclass
class Response:
    status: int
    body: Any = None
    error: str = ""


@dataclass
class AcceptanceReport:
    node_url: str
    symbol: str
    mode: str
    started_at: str
    finished_at: str = ""
    checks: list[CheckResult] = field(default_factory=list)
    order_results: list[dict[str, Any]] = field(default_factory=list)

    def add(self, name: str, status: str, detail: str = "", evidence: Any = None) -> None:
        self.checks.append(CheckResult(name, status, detail, evidence))
        print(f"[{status:4}] {name}: {detail}")

    def counts(self) -> dict[str, int]:
        result = {PASS: 0, FAIL: 0, WARN: 0, SKIP: 0}
        for check in self.checks:
            result[check.status] = result.get(check.status, 0) + 1
        return result

    def exit_code(self) -> int:
        return 1 if self.counts()[FAIL] else 0

    def to_dict(self) -> dict[str, Any]:
        return {
            "node_url": self.node_url,
            "symbol": self.symbol,
            "mode": self.mode,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "summary": self.counts(),
            "checks": [asdict(check) for check in self.checks],
            "orders": self.order_results,
        }

    def to_markdown(self) -> str:
        counts = self.counts()
        lines = [
            "# SimNow / CTP acceptance report",
            "",
            f"- Node: `{self.node_url}`",
            f"- Symbol: `{self.symbol}`",
            f"- Mode: `{self.mode}`",
            f"- Started: {self.started_at}",
            f"- Finished: {self.finished_at}",
            "",
            "## Summary",
            "",
            f"- PASS: {counts[PASS]}",
            f"- FAIL: {counts[FAIL]}",
            f"- WARN: {counts[WARN]}",
            f"- SKIP: {counts[SKIP]}",
            "",
            "## Checks",
            "",
            "| Status | Check | Detail |",
            "|---|---|---|",
        ]
        for check in self.checks:
            detail = check.detail.replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {check.status} | `{check.name}` | {detail} |")
        if self.order_results:
            lines += ["", "## Orders", "", "```json", json.dumps(self.order_results, indent=2), "```"]
        return "\n".join(lines) + "\n"


class NodeHttp:
    def __init__(self, base_url: str, timeout: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(self, method: str, path: str, body: Any = None) -> Response:
        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self.base_url + path, data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
                try:
                    parsed = json.loads(raw) if raw else None
                except json.JSONDecodeError:
                    parsed = raw.decode("utf-8", errors="replace")
                return Response(response.status, parsed)
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            try:
                parsed = json.loads(raw) if raw else None
            except json.JSONDecodeError:
                parsed = raw.decode("utf-8", errors="replace")
            return Response(exc.code, parsed, f"HTTP {exc.code}")
        except Exception as exc:  # noqa: BLE001 - probe reports, does not crash
            return Response(0, None, str(exc))

    def get(self, path: str) -> Response:
        return self.request("GET", path)

    def post(self, path: str, body: dict[str, Any]) -> Response:
        return self.request("POST", path, body)

    def delete(self, path: str) -> Response:
        return self.request("DELETE", path)


def finite(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def first_symbol(value: str) -> str:
    for item in value.split(","):
        if item.strip():
            return item.strip()
    return "SHFE.rb2701"


def align_down(value: float, tick: float) -> float:
    if tick <= 0:
        return value
    return math.floor(value / tick + 1e-9) * tick


def depth_levels(quote: dict[str, Any]) -> int:
    count = 0
    for name in ("bid_price", "ask_price"):
        for level in range(1, 6):
            if finite(quote.get(f"{name}{level}")) is not None:
                count += 1
    return count


class Acceptance:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.http = NodeHttp(args.node_url, args.timeout)
        self.report = AcceptanceReport(
            node_url=args.node_url,
            symbol=args.symbol,
            mode=args.mode,
            started_at=utc_now(),
        )
        self.quote: dict[str, Any] = {}
        self.order_ids: list[str] = []

    def check(self, name: str, status: str, detail: str = "", evidence: Any = None) -> None:
        self.report.add(name, status, detail, evidence)

    def run(self) -> int:
        try:
            self.check_health()
            ready = self.check_ready()
            self.check_account_positions()
            self.check_quote()
            if self.args.ws:
                self.check_websocket()
            if self.args.metrics:
                self.check_metrics()
            if ready and self.args.mode in ("orders", "orders-dry-run", "full"):
                self.run_order_checks()
            if not ready and self.args.mode in ("orders", "full"):
                self.check("orders", SKIP, "node 未 ready，跳过所有写操作")
        finally:
            self.report.finished_at = utc_now()
            self.write_reports()
        return self.report.exit_code()

    def check_health(self) -> None:
        response = self.http.get("/healthz")
        if response.status == 200 and isinstance(response.body, dict) and response.body.get("ok"):
            self.check("healthz", PASS, "HTTP 200，服务存活")
        else:
            self.check("healthz", FAIL, f"status={response.status} error={response.error}")

    def check_ready(self) -> bool:
        deadline = time.monotonic() + self.args.ready_timeout
        last: Response | None = None
        while time.monotonic() < deadline:
            last = self.http.get("/readyz")
            if last.status == 200 and isinstance(last.body, dict) and last.body.get("ready"):
                self.check("readyz", PASS, f"ready=true (waited {self.args.ready_timeout:.0f}s max)", last.body)
                return True
            time.sleep(0.5)
        detail = f"未在 {self.args.ready_timeout:.0f}s 内 ready"
        if last is not None:
            detail += f"，最后状态 HTTP {last.status}: {last.body}"
        self.check("readyz", FAIL, detail, last.body if last else None)
        return False

    def check_account_positions(self) -> None:
        for path, name in (
            ("/account", "account"),
            ("/positions", "positions"),
            ("/orders", "orders_snapshot"),
            ("/trades", "trades_snapshot"),
        ):
            response = self.http.get(path)
            if response.status != 200:
                self.check(name, FAIL, f"HTTP {response.status}: {response.error}")
                continue
            body = response.body
            if path == "/account":
                ok = isinstance(body, dict) and ("balance" in body or "available" in body)
            elif path == "/positions":
                ok = isinstance(body, dict) and isinstance(body.get("positions"), dict)
            else:
                key = "orders" if path == "/orders" else "trades"
                ok = isinstance(body, dict) and isinstance(body.get(key), dict)
            self.check(name, PASS if ok else FAIL, "字段结构正常" if ok else f"响应结构异常: {body}")

    def check_quote(self) -> None:
        # GET /quote 首次访问才会触发订阅，返回的空占位不是最终行情；
        # 新合约/新连接必须轮询等待首帧 quote，而不是立即判失败。
        deadline = time.monotonic() + self.args.quote_timeout
        last_response: Response | None = None
        last_body: Any = None
        while time.monotonic() < deadline:
            last_response = self.http.get(f"/quote/{self.args.symbol}")
            last_body = last_response.body
            if last_response.status == 200 and isinstance(last_body, dict):
                self.quote = last_body
                last = finite(last_body.get("last_price"))
                levels = depth_levels(last_body)
                if last is not None and levels >= 1:
                    break
                if last is not None and levels < 1:
                    # 有的合约先有最新价，盘口随后到达；继续等到超时。
                    pass
            time.sleep(0.5)

        if last_response is None or last_response.status != 200 or not isinstance(last_body, dict):
            status = last_response.status if last_response else 0
            error = last_response.error if last_response else "no response"
            self.check("quote", FAIL, f"HTTP {status}: {error} (waited {self.args.quote_timeout:.0f}s)", last_body)
            return
        self.quote = last_body
        last = finite(last_body.get("last_price"))
        tick = finite(last_body.get("price_tick"))
        levels = depth_levels(last_body)
        if last is None:
            self.check(
                "quote",
                FAIL,
                f"等待 {self.args.quote_timeout:.0f}s 后仍没有有限 last_price；"
                "合约可能无夜盘/未产生行情或订阅尚未返回",
                last_body,
            )
        elif levels < 1:
            self.check(
                "quote",
                FAIL,
                f"last={last}，但等待 {self.args.quote_timeout:.0f}s 后仍没有买卖盘口",
                last_body,
            )
        elif levels < 5:
            self.check(
                "quote",
                WARN,
                f"last={last} tick={tick} 仅 {levels} 个盘口价位；SimNow CTP 预期五档",
                last_body,
            )
        else:
            self.check("quote", PASS, f"last={last} tick={tick} depth_levels={levels}")

    def check_metrics(self) -> None:
        response = self.http.get("/metrics")
        if response.status == 200 and isinstance(response.body, dict):
            self.check("metrics", PASS, "metrics 可读取", response.body)
        else:
            self.check("metrics", WARN, f"metrics 不可用: HTTP {response.status}")

    def check_websocket(self) -> None:
        if websockets is None:
            self.check("websocket", SKIP, "Python websockets 未安装")
            return

        async def probe() -> tuple[bool, str, Any]:
            url = self.args.node_url.rstrip("/").replace("http://", "ws://").replace(
                "https://", "wss://"
            ) + "/stream"
            messages: list[dict[str, Any]] = []
            async with websockets.connect(url, open_timeout=self.args.timeout) as socket:
                await socket.send(
                    json.dumps(
                        {
                            "op": "subscribe",
                            "symbols": [self.args.symbol],
                            "klines": [],
                            "topics": [],
                        }
                    )
                )
                deadline = asyncio.get_running_loop().time() + self.args.ws_timeout
                while asyncio.get_running_loop().time() < deadline:
                    remaining = max(0.1, deadline - asyncio.get_running_loop().time())
                    raw = await asyncio.wait_for(socket.recv(), timeout=remaining)
                    try:
                        message = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    messages.append(message)
                    kind = message.get("type")
                    if kind in ("subscribed", "snapshot"):
                        return True, f"收到 {kind}，共 {len(messages)} 条消息", messages[-5:]
                    if kind == "error":
                        return False, f"WS error: {message}", messages[-5:]
                return False, f"{self.args.ws_timeout}s 内未收到 subscribed/snapshot", messages[-5:]

        try:
            ok, detail, evidence = asyncio.run(probe())
            self.check("websocket", PASS if ok else FAIL, detail, evidence)
        except Exception as exc:  # noqa: BLE001 - probe reports
            self.check("websocket", FAIL, f"WS 连接/订阅失败: {exc}")

    def next_limit_price(self) -> float | None:
        if self.args.limit_price is not None:
            return self.args.limit_price
        tick = finite(self.quote.get("price_tick")) or 1.0
        bid = finite(self.quote.get("bid_price1"))
        ask = finite(self.quote.get("ask_price1"))
        lower = finite(self.quote.get("lower_limit"))
        if self.args.direction == "BUY":
            reference = bid if bid is not None else ask
            if reference is None:
                reference = finite(self.quote.get("last_price"))
            if reference is None:
                return None
            target = reference - max(1.0, 3.0) * tick
            if lower is not None:
                target = max(target, lower + tick)
            return align_down(target, tick)
        reference = ask if ask is not None else bid
        if reference is None:
            reference = finite(self.quote.get("last_price"))
        if reference is None:
            return None
        target = reference + max(1.0, 3.0) * tick
        upper = finite(self.quote.get("upper_limit"))
        if upper is not None:
            target = min(target, upper - tick)
        return align_down(target, tick)

    def order_payload(self, *, limit_price: float | None, volume: int, error_case: bool = False) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "instrument_id": self.args.symbol,
            "direction": self.args.direction,
            "offset": self.args.offset,
            "price_type": "LIMIT",
            "time_cond": "GFD",
            "volume_cond": "ANY",
            "volume": volume,
        }
        if limit_price is not None:
            payload["limit_price"] = limit_price
        return payload

    def fetch_orders(self) -> dict[str, Any]:
        response = self.http.get("/orders")
        if response.status == 200 and isinstance(response.body, dict):
            orders = response.body.get("orders")
            return orders if isinstance(orders, dict) else {}
        return {}

    def fetch_order(self, order_id: str) -> dict[str, Any] | None:
        order = self.fetch_orders().get(order_id)
        return order if isinstance(order, dict) else None

    def wait_order(self, order_id: str, timeout: float) -> dict[str, Any] | None:
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.fetch_order(order_id)
            if last is not None:
                if last.get("is_dead") or last.get("is_error"):
                    return last
                if str(last.get("status")) == "FINISHED":
                    return last
            time.sleep(0.25)
        return last

    def record_order(self, scenario: str, payload: dict[str, Any], result: dict[str, Any]) -> None:
        self.report.order_results.append({"scenario": scenario, "payload": payload, "result": result})

    def cancel_and_wait(self, order_id: str) -> tuple[bool, str, Any]:
        response = self.http.delete(f"/order/{order_id}")
        evidence: dict[str, Any] = {"cancel_response": response.body}
        if response.status != 200:
            return False, f"撤单 HTTP {response.status}: {response.error}", evidence
        if not isinstance(response.body, dict) or not response.body.get("success"):
            return False, f"撤单失败: {response.body}", evidence
        order = self.wait_order(order_id, self.args.order_timeout)
        evidence["order"] = order
        if order is None:
            return False, "撤单后未查询到订单", evidence
        if order.get("is_error"):
            return False, f"订单异步失败: {order.get('last_msg')}", evidence
        if order.get("is_dead") or str(order.get("status")) == "FINISHED":
            return True, f"订单已终态: {order.get('last_msg') or order.get('status')}", evidence
        return True, "撤单被接受，但订单仍未终态", evidence

    def run_order_checks(self) -> None:
        if not self.args.yes:
            self.check("orders", FAIL, "写检查需要显式 --yes；请确认是 SimNow 小单验收")
            return
        price = self.next_limit_price()
        if price is None or price <= 0:
            self.check("orders", SKIP, "没有可用 quote/tick，无法计算远月限价")
            return
        payload = self.order_payload(limit_price=price, volume=self.args.order_volume)
        self.report.order_results.append({"scenario": "dry_payload", "payload": payload})
        if self.args.dry_run:
            self.check("orders.limit.cancel", SKIP, f"dry-run，仅打印订单 {payload}")
            return

        response = self.http.post("/order", payload)
        evidence: dict[str, Any] = {"submit_response": response.body}
        if response.status != 200 or not isinstance(response.body, dict):
            self.check("orders.limit.cancel", FAIL, f"下单 HTTP {response.status}: {response.error}", evidence)
            return
        order_id = str(response.body.get("order_id") or "")
        if not response.body.get("success") or not order_id:
            self.record_order("limit", payload, evidence)
            self.check("orders.limit.cancel", FAIL, f"限价单未被接受: {response.body}", evidence)
            return

        self.order_ids.append(order_id)
        submitted = self.wait_order(order_id, self.args.order_timeout)
        evidence["order_after_submit"] = submitted
        if submitted is None:
            self.check("orders.limit.cancel", FAIL, "下单 ack 后未查询到订单", evidence)
            return
        if submitted.get("is_error"):
            self.check("orders.limit.cancel", FAIL, f"限价单异步失败: {submitted.get('last_msg')}", evidence)
            return
        if submitted.get("is_dead"):
            self.check(
                "orders.limit.cancel",
                WARN,
                f"远月限价单仍终态（可能被柜台拒绝或快速成交）: {submitted.get('last_msg')}",
                evidence,
            )
            return

        ok, detail, cancel_evidence = self.cancel_and_wait(order_id)
        evidence.update(cancel_evidence)
        self.record_order("limit+cancel", payload, evidence)
        self.check("orders.limit.cancel", PASS if ok else FAIL, detail, evidence)

        error_payload = self.order_payload(limit_price=price, volume=0, error_case=True)
        error_response = self.http.post("/order", error_payload)
        error_evidence: dict[str, Any] = {"submit_response": error_response.body}
        self.record_order("reject", error_payload, error_evidence)
        reject_passed = False
        if error_response.status != 200 or not isinstance(error_response.body, dict):
            self.check("orders.reject", FAIL, f"错误单 HTTP {error_response.status}", error_evidence)
        elif not error_response.body.get("success"):
            reject_passed = True
            self.check("orders.reject", PASS, f"错误单被拒绝: {error_response.body.get('error')}", error_evidence)
        else:
            error_id = str(error_response.body.get("order_id") or "")
            if error_id:
                error_order = self.wait_order(error_id, self.args.order_timeout)
                error_evidence["order_after_submit"] = error_order
                if error_order is not None and error_order.get("is_error"):
                    reject_passed = True
                    self.check(
                        "orders.reject",
                        PASS,
                        f"错误单异步拒绝: {error_order.get('last_msg')}",
                        error_evidence,
                    )
                else:
                    if error_order is not None and not error_order.get("is_dead"):
                        self.cancel_and_wait(error_id)
            if not reject_passed:
                self.check("orders.reject", FAIL, f"volume=0 未被拒绝: {error_response.body}", error_evidence)

        # 错误单检查通过后仍需继续执行可选的市价/对价检查；不能 return，
        # 否则 --include-market/--include-counterparty 会被静默跳过。
        if self.args.include_market:
            self.run_optional_market("orders.market", "ANY")
        if self.args.include_counterparty:
            self.run_optional_market("orders.counterparty", "BEST")

    def run_optional_market(self, name: str, price_type: str) -> None:
        payload = self.order_payload(limit_price=None, volume=self.args.order_volume)
        payload["price_type"] = price_type
        payload["time_cond"] = "IOC"
        self.report.order_results.append({"scenario": name, "payload": payload})
        if self.args.dry_run:
            self.check(name, SKIP, f"dry-run，仅打印 {price_type} 订单")
            return
        response = self.http.post("/order", payload)
        evidence: dict[str, Any] = {"submit_response": response.body}
        if response.status != 200 or not isinstance(response.body, dict):
            self.check(name, FAIL, f"下单 HTTP {response.status}: {response.error}", evidence)
            return
        order_id = str(response.body.get("order_id") or "")
        if not response.body.get("success") or not order_id:
            self.record_order(name, payload, evidence)
            self.check(name, WARN, f"{price_type} 订单被柜台拒绝: {response.body.get('error')}", evidence)
            return
        order = self.wait_order(order_id, self.args.order_timeout)
        evidence["order_after_submit"] = order
        self.record_order(name, payload, evidence)
        if order is None:
            self.check(name, WARN, "订单 ack 后未查询到状态", evidence)
            return
        if order.get("is_error"):
            self.check(name, WARN, f"{price_type} 订单异步拒绝: {order.get('last_msg')}", evidence)
            return
        if order.get("is_dead") or str(order.get("status")) == "FINISHED":
            self.check(name, PASS, f"{price_type} 订单到达终态: {order.get('last_msg') or order.get('status')}", evidence)
            return
        ok, detail, cancel_evidence = self.cancel_and_wait(order_id)
        evidence.update(cancel_evidence)
        self.check(name, PASS if ok else WARN, f"{price_type} 订单仍存活，撤单结果: {detail}", evidence)

    def write_reports(self) -> None:
        if not self.args.write_report:
            return
        out_dir = Path(self.args.report_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        json_path = Path(self.args.json_out) if self.args.json_out else out_dir / f"simnow_acceptance_{stamp}.json"
        md_path = Path(self.args.md_out) if self.args.md_out else out_dir / f"simnow_acceptance_{stamp}.md"
        json_path.write_text(json.dumps(self.report.to_dict(), indent=2, ensure_ascii=False), encoding="utf-8")
        md_path.write_text(self.report.to_markdown(), encoding="utf-8")
        print(f"report: {json_path}")
        print(f"report: {md_path}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SimNow/CTP acceptance probe")
    parser.add_argument("--node-url", default=os.environ.get("TQSDK_NODE_URL", "http://127.0.0.1:18081"))
    parser.add_argument(
        "--symbol",
        default=first_symbol(os.environ.get("SYMBOLS", "SHFE.rb2701")),
        help="contract to check; default from SYMBOLS or SHFE.rb2701",
    )
    parser.add_argument("--orders", action="store_true", help="enable limit/cancel and rejection checks")
    parser.add_argument("--yes", action="store_true", help="confirm real SimNow write operations")
    parser.add_argument("--dry-run", action="store_true", help="print order payloads without submitting")
    parser.add_argument(
        "--include-market",
        action="store_true",
        help="also submit a small market order; may fill/create a position",
    )
    parser.add_argument(
        "--include-counterparty",
        action="store_true",
        help="also submit a small BEST-price order; may fill/create a position",
    )
    parser.add_argument("--ws", action="store_true", default=True, help="check WebSocket subscription")
    parser.add_argument("--no-ws", dest="ws", action="store_false")
    parser.add_argument("--metrics", action="store_true", help="also check /metrics")
    parser.add_argument("--direction", default="BUY", choices=["BUY", "SELL"])
    parser.add_argument("--offset", default="OPEN", choices=["OPEN", "CLOSE", "CLOSETODAY", "CLOSEYESTERDAY"])
    parser.add_argument("--order-volume", type=int, default=1)
    parser.add_argument("--limit-price", type=float, default=None)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument(
        "--quote-timeout",
        type=float,
        default=15.0,
        help="seconds to wait for the first non-empty quote after triggering subscription",
    )
    parser.add_argument("--ready-timeout", type=float, default=60.0)
    parser.add_argument("--ws-timeout", type=float, default=8.0)
    parser.add_argument("--order-timeout", type=float, default=8.0)
    parser.add_argument("--report-dir", default="artifacts")
    parser.add_argument("--json-out", default="")
    parser.add_argument("--md-out", default="")
    parser.add_argument("--no-report", dest="write_report", action="store_false", default=True)
    args = parser.parse_args(argv)
    args.mode = "orders" if args.orders else "read-only"
    if args.dry_run:
        args.mode = "orders-dry-run"
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.orders and not args.yes and not args.dry_run:
        print("拒绝执行写检查：请加 --yes 明确确认是 SimNow 小单验收。", file=sys.stderr)
        print("只读检查请去掉 --orders；预览订单请加 --dry-run。", file=sys.stderr)
        return 2
    if args.order_volume <= 0:
        print("--order-volume 必须大于 0", file=sys.stderr)
        return 2
    return Acceptance(args).run()


if __name__ == "__main__":
    raise SystemExit(main())
