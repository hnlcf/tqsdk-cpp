"""Shared EMA60 + Donchian22 + ATR strategy logic.

This module is intentionally independent of the C++/node transport. The same
`EmaDonchianStrategy.decide()` is used by the Python live runner; the existing
`scripts/backtest_daily.py` contains the batch research/report implementation
with the same signal rules.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


@dataclass
class Bar:
    datetime: int
    open: float
    high: float
    low: float
    close: float
    volume: float = 0.0

    @classmethod
    def from_value(cls, value: Any) -> "Bar":
        if isinstance(value, cls):
            return value
        if isinstance(value, dict):
            return cls(
                int(value["datetime"]),
                float(value["open"]),
                float(value["high"]),
                float(value["low"]),
                float(value["close"]),
                float(value.get("volume", 0.0)),
            )
        if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
            return cls(
                int(value[0]),
                float(value[1]),
                float(value[2]),
                float(value[3]),
                float(value[4]),
                float(value[5]) if len(value) > 5 else 0.0,
            )
        if hasattr(value, "datetime") and hasattr(value, "close"):
            return cls(
                int(value.datetime),
                float(value.open),
                float(value.high),
                float(value.low),
                float(value.close),
                float(getattr(value, "volume", 0.0)),
            )
        raise TypeError(f"cannot convert {type(value)!r} to Bar")


@dataclass
class ContractSpec:
    symbol: str
    volume_multiple: float
    margin_rate: float
    commission_per_lot: float = 0.0
    price_tick: float = 0.0

    @classmethod
    def from_dict(cls, symbol: str, value: dict[str, Any]) -> "ContractSpec":
        return cls(
            symbol=symbol,
            volume_multiple=float(value.get("volume_multiple", 0.0)),
            margin_rate=float(value.get("margin_rate_est", value.get("margin_rate", 0.0))),
            commission_per_lot=float(value.get("commission_per_lot", 0.0)),
            price_tick=float(value.get("price_tick", 0.0)),
        )


@dataclass
class StrategyConfig:
    symbol: str = "SHFE.rb2701"
    duration_ns: int = 86_400_000_000_000
    ema_period: int = 60
    donchian_period: int = 22
    atr_period: int = 14
    stop_atr: float = 2.0
    allocation: float = 0.30
    # For live startup/reconciliation, never risk more than this per task.
    max_volume: int = 0


@dataclass
class Decision:
    target: int
    reason: str
    signal: str = "flat"
    stop_price: float | None = None
    lots: int = 0


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


def load_specs(path: Path) -> dict[str, ContractSpec]:
    payload = json.loads(path.read_text())
    return {
        symbol: ContractSpec.from_dict(symbol, value)
        for symbol, value in payload.items()
        if isinstance(value, dict)
    }


def resolve_spec(specs: dict[str, ContractSpec], trading_symbol: str) -> ContractSpec | None:
    """Map an actual contract (SHFE.rb2701) to the KQ.i@ product spec."""
    if trading_symbol in specs:
        return specs[trading_symbol]
    if "." not in trading_symbol:
        return None
    exchange, contract = trading_symbol.split(".", 1)
    match = re.match(r"([A-Za-z]+)", contract)
    if not match:
        return None
    return specs.get(f"KQ.i@{exchange}.{match.group(1)}")


def load_daily_json(path: Path) -> list[Bar]:
    payload = json.loads(path.read_text())
    bars = [Bar.from_value(item) for item in payload.get("bars", [])]
    bars.sort(key=lambda item: item.datetime)
    dedup: list[Bar] = []
    for bar in bars:
        if dedup and dedup[-1].datetime == bar.datetime:
            dedup[-1] = bar
        else:
            dedup.append(bar)
    return dedup


class EmaDonchianStrategy:
    """Stateful strategy; decisions use only closed bars supplied by the caller."""

    def __init__(self, config: StrategyConfig, spec: ContractSpec):
        self.config = config
        self.spec = spec
        self.stop_price: float | None = None
        self.last_position = 0
        self.last_signal = "flat"
        self._pending_entry = False

    def decide(
        self,
        bars: Iterable[Any],
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

        # Reconcile a position opened before this process started, or anchor a
        # pending task's stop to the actual fill price once node reports it.
        if position != 0:
            side = 1 if position > 0 else -1
            if self._pending_entry and entry_price and entry_price > 0:
                self.stop_price = (
                    entry_price - side * self.config.stop_atr * atr_value
                )
                self._pending_entry = False
            elif self.last_position == 0 and self.stop_price is None:
                base = entry_price if entry_price and entry_price > 0 else last.close
                self.stop_price = base - side * self.config.stop_atr * atr_value

        if position != 0:
            side = 1 if position > 0 else -1
            # Quote-based stop for live polling. Backtest runners should use the
            # bar low/high for their own stop simulation.
            if price is not None and self.stop_price is not None:
                if (side > 0 and price <= self.stop_price) or (
                    side < 0 and price >= self.stop_price
                ):
                    self.stop_price = None
                    self._pending_entry = False
                    self.last_signal = "flat"
                    return Decision(0, "atr_stop", "flat")
            exit_signal = (
                side > 0
                and (last.close < ema_values[-1] or last.close < lower[-1])
            ) or (
                side < 0
                and (last.close > ema_values[-1] or last.close > upper[-1])
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


def target_from_live(
    config: StrategyConfig,
    spec: ContractSpec,
    bars: Iterable[Any],
    position: int,
    equity: float,
    quote: float | None = None,
    entry_price: float | None = None,
    strategy: EmaDonchianStrategy | None = None,
) -> Decision:
    """Convenience wrapper for a stateless call; callers should retain strategy."""
    strategy = strategy or EmaDonchianStrategy(config, spec)
    return strategy.decide(
        bars, position, equity, price=quote, entry_price=entry_price
    )
