"""Python live strategy adapter for tqsdk-node + CTP.

Topology:

    CTP -> tqsdk-gw -> tqsdk-node (REST/WS) -> this Python process

This module deliberately does not talk to CTP directly. It polls node for
closed bars/quotes/account/positions and submits net target positions through
`POST /task/target`; tqsdk-gw owns the CTP login and order lifecycle.

For the EMA60/Donchian22 daily/4h strategy, REST polling is sufficient and is
less fragile than consuming every market diff. The polling interval is only
the control latency; signal calculation is based on closed bars.

Safety defaults:
  * `execute=False`: only logs intended targets;
  * `/readyz` must report ready before a write;
  * target tasks are submitted only when the desired net position changes;
  * one process should own one account/symbol/strategy.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from tqsdk_strategy import (
    Bar,
    ContractSpec,
    Decision,
    EmaDonchianStrategy,
    StrategyConfig,
    load_specs,
    resolve_spec,
)


class NodeError(RuntimeError):
    def __init__(self, status: int, message: str, body: Any = None):
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.body = body


class NodeClient:
    def __init__(
        self,
        base_url: str = "http://127.0.0.1:18081",
        account: str = "default",
        token: str = "",
        timeout: float = 15.0,
    ):
        self.base_url = base_url.rstrip("/")
        self.account = account
        self.token = token or os.environ.get("TQSDK_SERVE_TOKEN", "")
        self.timeout = timeout

    def _url(self, path: str, account: bool = True) -> str:
        if not path.startswith("/"):
            path = "/" + path
        if account and "account=" not in path:
            sep = "&" if "?" in path else "?"
            path = f"{path}{sep}account={urllib.parse.quote(self.account)}"
        return self.base_url + path

    def _request(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        account: bool = True,
        allow_error_status: bool = False,
    ) -> tuple[int, Any]:
        data = None
        headers = {"Accept": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if body is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            self._url(path, account=account), data=data, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read()
                status = response.status
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            status = exc.code
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = raw.decode("utf-8", "replace")
        if status < 200 or status >= 300:
            if not allow_error_status:
                message = parsed.get("error") if isinstance(parsed, dict) else parsed
                raise NodeError(status, str(message), parsed)
        return status, parsed

    def readyz(self) -> tuple[int, dict[str, Any]]:
        status, body = self._request(
            "GET", "/readyz", allow_error_status=True
        )
        return status, body if isinstance(body, dict) else {}

    def is_ready(self) -> bool:
        try:
            status, body = self.readyz()
            return status == 200 and bool(body.get("ready"))
        except Exception:
            return False

    def quote(self, symbol: str) -> dict[str, Any]:
        _, body = self._request("GET", f"/quote/{urllib.parse.quote(symbol)}")
        return body if isinstance(body, dict) else {}

    def quotes(self, symbols: list[str]) -> dict[str, Any]:
        query = urllib.parse.urlencode({"symbols": ",".join(symbols)})
        _, body = self._request("GET", f"/quotes?{query}")
        return body if isinstance(body, dict) else {}

    def kline(
        self,
        symbol: str,
        duration_ns: int,
        width: int = 200,
        adjust: str = "none",
    ) -> list[Bar]:
        query = f"?width={int(width)}"
        if adjust and adjust != "none":
            query += f"&adjust={urllib.parse.quote(adjust)}"
        path = f"/kline/{urllib.parse.quote(symbol)}/{int(duration_ns)}{query}"
        _, body = self._request("GET", path)
        bars = body.get("bars", []) if isinstance(body, dict) else []
        return [Bar.from_value(item) for item in bars]

    def account(self) -> dict[str, Any]:
        _, body = self._request("GET", "/account")
        return body if isinstance(body, dict) else {}

    def positions(self) -> dict[str, Any]:
        _, body = self._request("GET", "/positions")
        return body if isinstance(body, dict) else {}

    def orders(self) -> dict[str, Any]:
        _, body = self._request("GET", "/orders")
        return body if isinstance(body, dict) else {}

    def trades(self) -> dict[str, Any]:
        _, body = self._request("GET", "/trades")
        return body if isinstance(body, dict) else {}

    def tasks(self) -> dict[str, Any]:
        _, body = self._request("GET", "/tasks")
        return body if isinstance(body, dict) else {}

    def target_position(
        self,
        symbol: str,
        target_volume: int,
        mode: str = "ACTIVE",
        max_volume: int = 0,
    ) -> dict[str, Any]:
        body = {
            "symbol": symbol,
            "target_volume": int(target_volume),
            "mode": mode,
        }
        if max_volume > 0:
            body["max_volume"] = int(max_volume)
        _, response = self._request("POST", "/task/target", body=body)
        return response if isinstance(response, dict) else {}

    def cancel_target(self, task_id: str) -> dict[str, Any]:
        path = f"/task/target/{urllib.parse.quote(task_id)}"
        _, response = self._request("DELETE", path)
        return response if isinstance(response, dict) else {}

    def insert_order(self, order: dict[str, Any]) -> dict[str, Any]:
        _, response = self._request("POST", "/order", body=order)
        return response if isinstance(response, dict) else {}


def net_position(position: dict[str, Any]) -> int:
    return int(position.get("volume_long", 0)) - int(position.get("volume_short", 0))


def entry_price(position: dict[str, Any]) -> float | None:
    if int(position.get("volume_long", 0)) > 0:
        value = position.get("open_price_long") or position.get("position_price_long")
        if value is not None:
            return float(value)
    if int(position.get("volume_short", 0)) > 0:
        value = position.get("open_price_short") or position.get("position_price_short")
        if value is not None:
            return float(value)
    return None


def closed_bars(bars: list[Bar], duration_ns: int, now_ns: int | None = None) -> list[Bar]:
    """Drop the latest bar unless its interval has definitely ended.

    Tq bar datetime is the bar start. If the timestamp is in the future/current
    interval, using it would make a live signal depend on a partial bar.
    """
    if not bars:
        return []
    now_ns = now_ns if now_ns is not None else time.time_ns()
    if bars[-1].datetime + duration_ns > now_ns:
        return bars[:-1]
    return bars


@dataclass
class LiveStats:
    cycles: int = 0
    decisions: int = 0
    submitted: int = 0
    skipped_not_ready: int = 0
    errors: int = 0


class LiveRunner:
    def __init__(
        self,
        client: NodeClient,
        strategy: EmaDonchianStrategy,
        config: StrategyConfig,
        spec: ContractSpec,
        *,
        execute: bool = False,
        poll_interval: float = 5.0,
        max_cycles: int | None = None,
        width: int = 200,
        logger=print,
    ):
        self.client = client
        self.strategy = strategy
        self.config = config
        self.spec = spec
        self.execute = execute
        self.poll_interval = max(0.1, poll_interval)
        self.max_cycles = max_cycles
        self.width = max(50, width)
        self.log = logger
        self.stats = LiveStats()
        self._pending_target: int | None = None
        self._last_closed_time: int | None = None

    def _position_info(self) -> tuple[int, float | None]:
        payload = self.client.positions()
        positions = payload.get("positions", {})
        item = positions.get(self.config.symbol, {}) if isinstance(positions, dict) else {}
        if not isinstance(item, dict):
            return 0, None
        return net_position(item), entry_price(item)

    def _equity(self) -> float:
        account = self.client.account()
        value = account.get("balance")
        if value is None:
            value = account.get("static_balance")
        if value is None:
            value = account.get("available")
        return float(value) if value is not None else 0.0

    def _quote_price(self) -> float | None:
        quote = self.client.quote(self.config.symbol)
        value = quote.get("last_price")
        if value is None:
            value = quote.get("bid_price1") or quote.get("ask_price1")
        try:
            return float(value) if value is not None else None
        except (TypeError, ValueError):
            return None

    def _submit(self, target: int, decision_reason: str) -> None:
        if self._pending_target == target:
            return
        if not self.execute:
            self.log(
                json.dumps(
                    {
                        "dry_run": True,
                        "symbol": self.config.symbol,
                        "target_volume": target,
                        "reason": decision_reason,
                    },
                    ensure_ascii=False,
                )
            )
            self._pending_target = target
            return
        response = self.client.target_position(
            self.config.symbol,
            target,
            mode="ACTIVE",
            max_volume=self.config.max_volume,
        )
        ok = bool(response.get("success", True))
        self.stats.submitted += 1
        self.log(
            json.dumps(
                {
                    "submitted": ok,
                    "symbol": self.config.symbol,
                    "target_volume": target,
                    "task_id": response.get("task_id"),
                    "reason": decision_reason,
                },
                ensure_ascii=False,
            )
        )
        if ok:
            self._pending_target = target

    def step(self) -> Decision | None:
        self.stats.cycles += 1
        status, ready = self.client.readyz()
        if status != 200 or not ready.get("ready"):
            self.stats.skipped_not_ready += 1
            self.log(
                json.dumps(
                    {
                        "ready": False,
                        "status": status,
                        "phase": ready.get("td", {}).get("phase"),
                    },
                    ensure_ascii=False,
                )
            )
            return None

        bars = self.client.kline(
            self.config.symbol, self.config.duration_ns, width=self.width
        )
        bars = closed_bars(bars, self.config.duration_ns)
        if not bars:
            return None
        position, opened_at = self._position_info()
        equity = self._equity()
        quote_price = self._quote_price()
        decision = self.strategy.decide(
            bars,
            position,
            equity,
            price=quote_price,
            entry_price=opened_at,
        )
        self.stats.decisions += 1
        if decision.target != position:
            self._submit(decision.target, decision.reason)
        self.log(
            json.dumps(
                {
                    "symbol": self.config.symbol,
                    "position": position,
                    "equity": equity,
                    "last_bar": bars[-1].datetime,
                    "close": bars[-1].close,
                    "quote": quote_price,
                    "target": decision.target,
                    "signal": decision.signal,
                    "reason": decision.reason,
                    "stop": self.strategy.stop_price,
                },
                ensure_ascii=False,
            )
        )
        return decision

    def run(self) -> LiveStats:
        running = True

        def stop_handler(signum, frame):  # noqa: ARG001
            nonlocal running
            running = False

        previous = signal.signal(signal.SIGINT, stop_handler)
        try:
            while running:
                if self.max_cycles is not None and self.stats.cycles >= self.max_cycles:
                    break
                try:
                    self.step()
                except Exception as exc:  # noqa: BLE001 - long-running runner reports and continues
                    self.stats.errors += 1
                    self.log(json.dumps({"error": str(exc)}, ensure_ascii=False))
                if self.max_cycles is not None and self.stats.cycles >= self.max_cycles:
                    break
                time.sleep(self.poll_interval)
        finally:
            signal.signal(signal.SIGINT, previous)
        return self.stats


def build_runner(args: argparse.Namespace) -> LiveRunner:
    specs = load_specs(Path(args.spec_file))
    spec = resolve_spec(specs, args.symbol)
    if spec is None:
        raise SystemExit(f"no spec for {args.symbol} in {args.spec_file}")
    config = StrategyConfig(
        symbol=args.symbol,
        duration_ns=args.duration_ns,
        ema_period=args.ema,
        donchian_period=args.donchian,
        atr_period=args.atr,
        stop_atr=args.stop_atr,
        allocation=args.allocation,
        max_volume=args.max_volume,
    )
    client = NodeClient(
        base_url=args.node_url,
        account=args.account,
        token=args.token,
    )
    strategy = EmaDonchianStrategy(config, spec)
    return LiveRunner(
        client,
        strategy,
        config,
        spec,
        execute=args.execute,
        poll_interval=args.poll_interval,
        max_cycles=args.max_cycles,
        width=args.width,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run EMA60/Donchian22/ATR against tqsdk-node")
    parser.add_argument("--node-url", default="http://127.0.0.1:18081")
    parser.add_argument("--account", default="default")
    parser.add_argument("--token", default=os.environ.get("TQSDK_SERVE_TOKEN", ""))
    parser.add_argument("--symbol", default="SHFE.rb2701")
    parser.add_argument("--duration-ns", type=int, default=86_400_000_000_000)
    parser.add_argument("--spec-file", default="config/daily_backtest_specs.json")
    parser.add_argument("--ema", type=int, default=60)
    parser.add_argument("--donchian", type=int, default=22)
    parser.add_argument("--atr", type=int, default=14)
    parser.add_argument("--stop-atr", type=float, default=2.0)
    parser.add_argument("--allocation", type=float, default=0.30)
    parser.add_argument("--max-volume", type=int, default=0)
    parser.add_argument("--poll-interval", type=float, default=5.0)
    parser.add_argument("--width", type=int, default=200)
    parser.add_argument("--max-cycles", type=int, default=0)
    parser.add_argument("--execute", action="store_true", help="actually submit target tasks")
    args = parser.parse_args(argv)
    args.max_cycles = None if args.max_cycles <= 0 else args.max_cycles

    runner = build_runner(args)
    stats = runner.run()
    print(json.dumps(stats.__dict__, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
