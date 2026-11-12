"""Backtest adapter around the C++ BacktestEngine.

The generic bar adapter executes target-position signals at the C++ engine's
configured fill price (bar close by default). For the exact multi-product
research methodology with intrabar ATR stops, use `scripts/backtest_daily.py`;
that runner remains the reporting source of truth.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

import tqsdk_backtest as cpp

from .data import BarSource
from .models import Bar, ContractSpec, Signal, StrategyContext
from .signals import DispatchResult, SignalDispatcher, SignalConsumer
from .strategy import Strategy


@dataclass
class BacktestSettings:
    initial_balance: float = 1_000_000.0
    commission_rate: float = 0.0
    execute_on_bar_close: bool = True
    margin_rate_override: float | None = None


@dataclass
class BacktestRunResult:
    symbol: str
    bars: int
    signals: list[Signal] = field(default_factory=list)
    dispatches: list[DispatchResult] = field(default_factory=list)
    engine_result: Any = None

    @property
    def stats(self) -> Any:
        return self.engine_result.stats if self.engine_result is not None else None

    @property
    def fills(self) -> Any:
        return self.engine_result.fills if self.engine_result is not None else []


class BacktestRunner:
    def __init__(
        self,
        data_source: BarSource,
        strategy: Strategy,
        spec: ContractSpec,
        settings: BacktestSettings | None = None,
        *,
        notifiers: list[SignalConsumer] | None = None,
        logger: Callable[[str], None] | None = None,
        width: int = 200,
    ):
        self.data_source = data_source
        self.strategy = strategy
        self.spec = spec
        self.settings = settings or BacktestSettings()
        self.dispatcher = SignalDispatcher(notifiers or [], executor=None)
        self.logger = logger
        self.width = width

    def run(
        self,
        symbol: str,
        duration_ns: int = 86_400_000_000_000,
        *,
        width: int | None = None,
    ) -> BacktestRunResult:
        bars = self.data_source.load(symbol, duration_ns, width or self.width)
        if not bars:
            raise ValueError(f"no bars for {symbol}")

        config = cpp.BacktestConfig(
            initial_balance=self.settings.initial_balance,
            volume_multiple=self.spec.volume_multiple,
            margin_rate=(
                self.settings.margin_rate_override
                if self.settings.margin_rate_override is not None
                else self.spec.margin_rate
            ),
            commission_rate=self.settings.commission_rate,
            commission_per_lot=self.spec.commission_per_lot,
            execute_on_bar_close=self.settings.execute_on_bar_close,
        )
        engine = cpp.BacktestEngine(config)
        history: list[Bar] = []
        signals: list[Signal] = []
        dispatches: list[DispatchResult] = []

        def callback(context_cpp: Any, cpp_bar: Any) -> None:
            bar = Bar(
                int(cpp_bar.datetime),
                float(cpp_bar.open),
                float(cpp_bar.high),
                float(cpp_bar.low),
                float(cpp_bar.close),
                int(cpp_bar.volume),
            )
            history.append(bar)
            strategy_context = StrategyContext(
                symbol=symbol,
                bars=tuple(history),
                position=int(context_cpp.position()),
                equity=float(context_cpp.equity()),
                spec=self.spec,
                price=bar.close,
            )
            signal = self.strategy.on_bar(strategy_context)
            if signal is None:
                return
            signals.append(signal)
            dispatch = self.dispatcher.dispatch(signal, strategy_context)
            dispatches.append(dispatch)
            context_cpp.target_position(signal.target_volume)
            self.strategy.ack(signal)

        try:
            result = engine.run(
                [
                    cpp.Bar(
                        bar.datetime,
                        bar.open,
                        bar.high,
                        bar.low,
                        bar.close,
                        int(bar.volume),
                    )
                    for bar in bars
                ],
                callback,
            )
        finally:
            engine.close()
        return BacktestRunResult(
            symbol=symbol,
            bars=len(bars),
            signals=signals,
            dispatches=dispatches,
            engine_result=result,
        )
