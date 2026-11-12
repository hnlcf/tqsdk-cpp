"""Small dependency-free indicator helpers used by strategies."""

from __future__ import annotations

import math

from .models import Bar, ContractSpec


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
