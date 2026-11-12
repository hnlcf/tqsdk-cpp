"""Strategy interfaces and the EMA60/Donchian22/ATR implementation."""

from __future__ import annotations

import math
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Iterable

from .models import Bar, ContractSpec, Signal, StrategyContext


def ema(values: list[float], period: int) -> list[float]:
    alpha = 2.0 / (period + 1.0)
    out: list[float] = []
    current = values[0] if values else math.nan
    for i, value in enumerate(values):
        current = value if i == 0 else current + alpha * (value - current)
        out.append(current)
    return out


def atr(bars: list[Bar], period: int) -> list[float]:
    out = [math.nan] * len(bars)
    trs: list[float] = []
    for i, bar in enumerate(bars):
        if i == 0:
            tr = bar.high - bar.low
        else:
            prev = bars[i - 1].close
            tr = max(bar.high - bar.low, abs(bar.high - prev), abs(bar.low - prev))
        trs.append(tr)
        if i >= period - 1:
            out[i] = sum(trs[i + 1 - period : i + 1]) / period
    return out


def donchian(bars: list[Bar], period: int) -> tuple[list[float], list[float]]:
    upper = [math.nan] * len(bars)
    lower = [math.nan] * len(bars)
    for i in range(period, len(bars)):
        window = bars[i - period : i]
        upper[i] = max(item.high for item in window)
        lower[i] = min(item.low for item in window)
    return upper, lower


def lots_for_margin(
    equity: float,
    price: float,
    spec: ContractSpec,
    allocation: float,
) -> int:
    if equity <= 0 or price <= 0 or spec.volume_multiple <= 0 or spec.margin_rate <= 0:
        return 0
    margin_per_lot = price * spec.volume_multiple * spec.margin_rate
    return max(0, int((equity * allocation) / margin_per_lot))


@dataclass
class StrategyConfig:
    symbol: str = "SHFE.rb2701"
    duration_ns: int = 86_400_000_000_000
    ema_period: int = 60
    donchian_period: int = 22
    atr_period: int = 14
    stop_atr: float = 2.0
    allocation: float = 0.30
    max_volume: int = 0


@dataclass
class Decision:
    target: int
    reason: str
    signal: str = "flat"
    stop_price: float | None = None
    lots: int = 0


class Strategy(ABC):
    """Strategy interface shared by backtest and live runners."""

    @abstractmethod
    def on_bar(self, context: StrategyContext) -> Signal | None:
        """Return a signal when the desired target changes, else None."""

    def reset(self) -> None:
        """Optional reset hook for a new run."""

    def ack(self, signal: Signal) -> None:
        """Called after a signal was successfully dispatched/executed."""


class EmaDonchianStrategy(Strategy):
    """Stateful implementation; decisions use only closed bars supplied by caller."""

    def __init__(self, config: StrategyConfig, spec: ContractSpec):
        self.config = config
        self.spec = spec
        self.stop_price: float | None = None
        self.last_position = 0
        self.last_signal = "flat"
        self._pending_entry = False
        self._last_emitted_target = 0

    @property
    def current_target(self) -> int:
        return self._last_emitted_target

    def reset(self) -> None:
        self.stop_price = None
        self.last_position = 0
        self.last_signal = "flat"
        self._pending_entry = False
        self._last_emitted_target = 0

    def ack(self, signal: Signal) -> None:
        self._last_emitted_target = signal.target_volume

    def decide(
        self,
        bars: Iterable[Bar],
        position: int,
        equity: float,
        *,
        price: float | None = None,
        entry_price: float | None = None,
    ) -> Decision:
        bars = [Bar.from_value(item) for item in bars]
        warmup = max(
            self.config.ema_period,
            self.config.donchian_period,
            self.config.atr_period,
        ) + 1
        if len(bars) < warmup:
            return Decision(position, "insufficient bars", "flat")

        closes = [bar.close for bar in bars]
        ema_values = ema(closes, self.config.ema_period)
        upper, lower = donchian(bars, self.config.donchian_period)
        atr_values = atr(bars, self.config.atr_period)
        last = bars[-1]
        atr_value = atr_values[-1]
        if not math.isfinite(atr_value) or atr_value <= 0:
            return Decision(position, "ATR unavailable", "flat")

        if position != 0:
            side = 1 if position > 0 else -1
            if self._pending_entry and entry_price and entry_price > 0:
                self.stop_price = entry_price - side * self.config.stop_atr * atr_value
                self._pending_entry = False
            elif self.last_position == 0 and self.stop_price is None:
                base = entry_price if entry_price and entry_price > 0 else last.close
                self.stop_price = base - side * self.config.stop_atr * atr_value

            if price is not None and self.stop_price is not None:
                if (side > 0 and price <= self.stop_price) or (
                    side < 0 and price >= self.stop_price
                ):
                    self.stop_price = None
                    self._pending_entry = False
                    self.last_signal = "flat"
                    return Decision(0, "atr_stop", "flat")

            exit_signal = (
                side > 0 and (last.close < ema_values[-1] or last.close < lower[-1])
            ) or (
                side < 0 and (last.close > ema_values[-1] or last.close > upper[-1])
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

        long_signal = last.close > ema_values[-1] and last.close > upper[-1]
        short_signal = last.close < ema_values[-1] and last.close < lower[-1]
        if not long_signal and not short_signal:
            self.stop_price = None
            self.last_position = 0
            self.last_signal = "flat"
            return Decision(0, "no entry", "flat")

        lots = lots_for_margin(equity, last.close, self.spec, self.config.allocation)
        if self.config.max_volume > 0:
            lots = min(lots, self.config.max_volume)
        if lots <= 0:
            self.last_signal = "long" if long_signal else "short"
            return Decision(0, "one lot exceeds margin budget", self.last_signal, None, 0)
        side = 1 if long_signal else -1
        self.stop_price = last.close - side * self.config.stop_atr * atr_value
        self._pending_entry = True
        self.last_position = side * lots
        self.last_signal = "long" if side > 0 else "short"
        return Decision(side * lots, "breakout", self.last_signal, self.stop_price, lots)

    def on_bar(self, context: StrategyContext) -> Signal | None:
        decision = self.decide(
            context.bars,
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
            timestamp_ns=context.now_ns or (context.bars[-1].datetime if context.bars else 0),
            strategy_id=type(self).__name__,
            metadata={
                "allocation": self.config.allocation,
                "stop_atr": self.config.stop_atr,
            },
        )
