"""Live strategy runner built from data/strategy/signal/execution adapters."""

from __future__ import annotations

import json
import signal as system_signal
import time
from dataclasses import dataclass
from typing import Any, Callable

from .data import BarSource
from .models import StrategyContext
from .node import NodeClient, entry_price, net_position
from .signals import DispatchResult, SignalDispatcher
from .strategy import Strategy


@dataclass
class LiveConfig:
    symbol: str = "SHFE.rb2701"
    duration_ns: int = 86_400_000_000_000
    poll_interval: float = 5.0
    width: int = 200
    max_cycles: int | None = None


@dataclass
class LiveStats:
    cycles: int = 0
    signals: int = 0
    submitted: int = 0
    skipped_not_ready: int = 0
    consumer_errors: int = 0
    errors: int = 0


class LiveRunner:
    """Poll node, build a StrategyContext, dispatch changed signals.

    The runner is deliberately transport-agnostic: a data source, a node-like
    account/position/readiness source and a SignalDispatcher are injected. The
    production wiring uses NodeClient/NodeBarSource/SignalDispatcher with
    NodeTargetExecutor and optional WeComConsumer.
    """

    def __init__(
        self,
        node: NodeClient,
        data_source: BarSource,
        strategy: Strategy,
        spec: Any,
        dispatcher: SignalDispatcher,
        config: LiveConfig | None = None,
        *,
        logger: Callable[[str], None] = print,
    ):
        self.node = node
        self.data_source = data_source
        self.strategy = strategy
        self.spec = spec
        self.dispatcher = dispatcher
        self.config = config or LiveConfig()
        self.log = logger
        self.stats = LiveStats()
        self.dispatches: list[DispatchResult] = []

    def _position_info(self) -> tuple[int, float | None]:
        payload = self.node.positions()
        positions = payload.get("positions", {})
        item = positions.get(self.config.symbol, {}) if isinstance(positions, dict) else {}
        if not isinstance(item, dict):
            return 0, None
        return net_position(item), entry_price(item)

    def _equity(self) -> float:
        account = self.node.account()
        value = account.get("balance")
        if value is None:
            value = account.get("static_balance")
        if value is None:
            value = account.get("available")
        return float(value) if value is not None else 0.0

    def _quote_price(self) -> float | None:
        quote = self.node.quote(self.config.symbol)
        value = quote.get("last_price")
        if value is None:
            value = quote.get("bid_price1") or quote.get("ask_price1")
        try:
            return float(value) if value is not None else None
        except (TypeError, ValueError):
            return None

    def step(self) -> DispatchResult | None:
        self.stats.cycles += 1
        status, ready = self.node.readyz()
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

        bars = self.data_source.load(
            self.config.symbol, self.config.duration_ns, self.config.width
        )
        if not bars:
            return None
        position, opened_at = self._position_info()
        equity = self._equity()
        quote_price = self._quote_price()
        context = StrategyContext(
            symbol=self.config.symbol,
            bars=tuple(bars),
            position=position,
            equity=equity,
            spec=self.spec,
            price=quote_price,
            entry_price=opened_at,
            now_ns=time.time_ns(),
        )
        signal = self.strategy.on_bar(context)
        if signal is not None and signal.target_volume == position:
            # Reconciliation signal: the strategy now knows the actual
            # position, but there is no order action to execute/notify.
            self.strategy.ack(signal)
            signal = None
        if signal is None:
            self.log(
                json.dumps(
                    {
                        "symbol": self.config.symbol,
                        "position": position,
                        "equity": equity,
                        "bar": bars[-1].datetime,
                        "quote": quote_price,
                        "signal": None,
                    },
                    ensure_ascii=False,
                )
            )
            return None

        self.stats.signals += 1
        dispatch = self.dispatcher.dispatch(signal, context)
        self.dispatches.append(dispatch)
        self.stats.consumer_errors += len(dispatch.errors)
        execution = dispatch.execution
        if execution is None or getattr(execution, "accepted", False) or getattr(execution, "dry_run", False):
            self.strategy.ack(signal)
        if execution is not None and getattr(execution, "accepted", False):
            self.stats.submitted += 1
        self.log(
            json.dumps(
                {
                    "symbol": signal.symbol,
                    "target": signal.target_volume,
                    "signal": signal.signal,
                    "reason": signal.reason,
                    "stop": signal.stop_price,
                    "consumer_errors": len(dispatch.errors),
                    "task_id": getattr(dispatch.execution, "task_id", None),
                },
                ensure_ascii=False,
            )
        )
        return dispatch

    def run(self) -> LiveStats:
        running = True

        def stop_handler(signum, frame):  # noqa: ARG001
            nonlocal running
            running = False

        previous = system_signal.signal(system_signal.SIGINT, stop_handler)
        try:
            while running:
                if self.config.max_cycles is not None and self.stats.cycles >= self.config.max_cycles:
                    break
                try:
                    self.step()
                except Exception as exc:  # noqa: BLE001 - daemon loop must continue
                    self.stats.errors += 1
                    self.log(json.dumps({"error": str(exc)}, ensure_ascii=False))
                if self.config.max_cycles is not None and self.stats.cycles >= self.config.max_cycles:
                    break
                time.sleep(max(0.1, self.config.poll_interval))
        finally:
            system_signal.signal(system_signal.SIGINT, previous)
        return self.stats
