"""EMA60 + Donchian22 + ATR14 strategy.

This module contains strategy logic only: no CLI, node client, file I/O,
notifier, or execution code. The framework creates and runs it through the
strategy registry.

`on_bar()` keeps incremental EMA/ATR/Donchian state and only consumes newly
appended bars. The strategy interface remains a list/Sequence of Bar objects;
no numpy/pandas dependency is required. `decide()` is retained as a one-shot
stateless-from-the-caller API for tests/research callers and recomputes arrays.
"""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass
from typing import Iterable

from ..indicators import atr, donchian, ema, lots_for_margin
from ..models import Bar, ContractSpec, Signal, StrategyContext
from ..registry import register_strategy
from ..strategy import Strategy


@dataclass
class Decision:
    target: int
    reason: str
    signal: str = "flat"
    stop_price: float | None = None
    lots: int = 0


@register_strategy(
    "ema_donchian",
    aliases=("ema", "ema60", "ema_donchian_atr"),
    defaults={
        "ema_period": 60,
        "donchian_period": 22,
        "atr_period": 14,
        "stop_atr": 2.0,
        "allocation": 0.30,
        "max_volume": 0,
    },
    description="EMA60 + Donchian22 breakout with 2*ATR14 stop",
)
class EmaDonchianStrategy(Strategy):
    """EMA/Donchian/ATR strategy with incremental indicator state."""

    def __init__(
        self,
        *,
        spec: ContractSpec,
        symbol: str = "SHFE.rb2701",
        duration_ns: int = 86_400_000_000_000,
        ema_period: int = 60,
        donchian_period: int = 22,
        atr_period: int = 14,
        stop_atr: float = 2.0,
        allocation: float = 0.30,
        max_volume: int = 0,
    ):
        self.spec = spec
        self.symbol = symbol
        self.duration_ns = duration_ns
        self.ema_period = int(ema_period)
        self.donchian_period = int(donchian_period)
        self.atr_period = int(atr_period)
        self.stop_atr = float(stop_atr)
        self.allocation = float(allocation)
        self.max_volume = int(max_volume)
        self.stop_price: float | None = None
        self.last_position = 0
        self.last_signal = "flat"
        self._pending_entry = False
        self._last_emitted_target = 0
        self._reset_incremental()

    def _reset_incremental(self) -> None:
        self._inc_bars: list[Bar] = []
        self._inc_ema: float = math.nan
        self._inc_trs: deque[float] = deque(maxlen=max(1, self.atr_period))
        self._inc_atr: float = math.nan
        self._inc_highs: deque[float] = deque(maxlen=max(1, self.donchian_period))
        self._inc_lows: deque[float] = deque(maxlen=max(1, self.donchian_period))
        self._inc_upper: float = math.nan
        self._inc_lower: float = math.nan

    @property
    def current_target(self) -> int:
        return self._last_emitted_target

    def reset(self) -> None:
        self.stop_price = None
        self.last_position = 0
        self.last_signal = "flat"
        self._pending_entry = False
        self._last_emitted_target = 0
        self._reset_incremental()

    def ack(self, signal: Signal) -> None:
        self._last_emitted_target = signal.target_volume

    @staticmethod
    def _as_bars(bars: Iterable[Bar]) -> list[Bar]:
        # Runners pass a list; keep the common path allocation-free.
        if isinstance(bars, list) and (not bars or isinstance(bars[-1], Bar)):
            return bars
        return [Bar.from_value(item) for item in bars]

    def _push_bar(self, bar: Bar) -> None:
        if self._inc_bars:
            prev_close = self._inc_bars[-1].close
            tr = max(
                bar.high - bar.low,
                abs(bar.high - prev_close),
                abs(bar.low - prev_close),
            )
        else:
            tr = bar.high - bar.low

        # Donchian upper/lower use the *previous* N bars, matching indicators.donchian.
        if len(self._inc_highs) >= self.donchian_period:
            self._inc_upper = max(self._inc_highs)
            self._inc_lower = min(self._inc_lows)
        else:
            self._inc_upper = math.nan
            self._inc_lower = math.nan
        self._inc_highs.append(bar.high)
        self._inc_lows.append(bar.low)

        alpha = 2.0 / (self.ema_period + 1.0)
        if not self._inc_bars:
            self._inc_ema = bar.close
        else:
            self._inc_ema += alpha * (bar.close - self._inc_ema)

        self._inc_trs.append(tr)
        if len(self._inc_trs) >= self.atr_period:
            self._inc_atr = sum(self._inc_trs) / self.atr_period
        else:
            self._inc_atr = math.nan

        self._inc_bars.append(bar)

    def _ingest(self, bars: list[Bar]) -> None:
        if not self._inc_bars:
            start = 0
        elif (
            len(bars) < len(self._inc_bars)
            or bars[0].datetime != self._inc_bars[0].datetime
            or bars[len(self._inc_bars) - 1].datetime != self._inc_bars[-1].datetime
        ):
            # A new series, a sliding window, or a different history stream.
            self._reset_incremental()
            start = 0
        else:
            start = len(self._inc_bars)
        for index in range(start, len(bars)):
            self._push_bar(bars[index])

    def _decide_from_values(
        self,
        last: Bar,
        ema_value: float,
        upper: float,
        lower: float,
        atr_value: float,
        position: int,
        equity: float,
        *,
        price: float | None,
        entry_price: float | None,
    ) -> Decision:
        if not math.isfinite(atr_value) or atr_value <= 0:
            return Decision(position, "ATR unavailable", "flat")

        if position != 0:
            side = 1 if position > 0 else -1
            if self._pending_entry and entry_price and entry_price > 0:
                self.stop_price = entry_price - side * self.stop_atr * atr_value
                self._pending_entry = False
            elif self.last_position == 0 and self.stop_price is None:
                base = entry_price if entry_price and entry_price > 0 else last.close
                self.stop_price = base - side * self.stop_atr * atr_value

            if price is not None and self.stop_price is not None:
                if (side > 0 and price <= self.stop_price) or (
                    side < 0 and price >= self.stop_price
                ):
                    self.stop_price = None
                    self._pending_entry = False
                    self.last_signal = "flat"
                    return Decision(0, "atr_stop", "flat")

            exit_signal = (
                side > 0 and (last.close < ema_value or last.close < lower)
            ) or (
                side < 0 and (last.close > ema_value or last.close > upper)
            )
            if exit_signal:
                self.stop_price = None
                self._pending_entry = False
                self.last_signal = "flat"
                return Decision(0, "channel_or_ema", "flat")
            self.last_position = position
            return Decision(
                position,
                "hold",
                "long" if side > 0 else "short",
                self.stop_price,
                0,
            )

        long_signal = last.close > ema_value and last.close > upper
        short_signal = last.close < ema_value and last.close < lower
        if not long_signal and not short_signal:
            self.stop_price = None
            self.last_position = 0
            self.last_signal = "flat"
            return Decision(0, "no entry", "flat")

        lots = lots_for_margin(equity, last.close, self.spec, self.allocation)
        if self.max_volume > 0:
            lots = min(lots, self.max_volume)
        if lots <= 0:
            self.last_signal = "long" if long_signal else "short"
            return Decision(0, "one lot exceeds margin budget", self.last_signal, None, 0)
        side = 1 if long_signal else -1
        self.stop_price = last.close - side * self.stop_atr * atr_value
        self._pending_entry = True
        self.last_position = side * lots
        self.last_signal = "long" if side > 0 else "short"
        return Decision(side * lots, "breakout", self.last_signal, self.stop_price, lots)

    def _incremental_decide(
        self,
        bars: list[Bar],
        position: int,
        equity: float,
        *,
        price: float | None = None,
        entry_price: float | None = None,
    ) -> Decision:
        self._ingest(bars)
        warmup = max(self.ema_period, self.donchian_period, self.atr_period) + 1
        if len(bars) < warmup:
            return Decision(position, "insufficient bars", "flat")
        return self._decide_from_values(
            bars[-1],
            self._inc_ema,
            self._inc_upper,
            self._inc_lower,
            self._inc_atr,
            position,
            equity,
            price=price,
            entry_price=entry_price,
        )

    def decide(
        self,
        bars: Iterable[Bar],
        position: int,
        equity: float,
        *,
        price: float | None = None,
        entry_price: float | None = None,
    ) -> Decision:
        """One-shot calculation API (recomputes arrays; runners use on_bar)."""
        bars = [Bar.from_value(item) for item in bars]
        warmup = max(self.ema_period, self.donchian_period, self.atr_period) + 1
        if len(bars) < warmup:
            return Decision(position, "insufficient bars", "flat")

        closes = [bar.close for bar in bars]
        ema_values = ema(closes, self.ema_period)
        upper, lower = donchian(bars, self.donchian_period)
        atr_values = atr(bars, self.atr_period)
        return self._decide_from_values(
            bars[-1],
            ema_values[-1],
            upper[-1],
            lower[-1],
            atr_values[-1],
            position,
            equity,
            price=price,
            entry_price=entry_price,
        )

    def on_bar(self, context: StrategyContext) -> Signal | None:
        bars = self._as_bars(context.bars)
        if not bars:
            return None
        decision = self._incremental_decide(
            bars,
            context.position,
            context.equity,
            price=context.price,
            entry_price=context.entry_price,
        )
        if decision.target == self._last_emitted_target:
            return None
        return Signal(
            symbol=context.symbol,
            target_volume=decision.target,
            reason=decision.reason,
            signal=decision.signal,
            stop_price=decision.stop_price,
            lots=decision.lots,
            timestamp_ns=context.now_ns or bars[-1].datetime,
            strategy_id=type(self).__name__,
            metadata={"allocation": self.allocation, "stop_atr": self.stop_atr},
        )
