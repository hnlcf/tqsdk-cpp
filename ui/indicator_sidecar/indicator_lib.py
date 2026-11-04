"""Shared indicator helpers and colors for TQSDK Python plugins.

Plugins can simply use:

    from indicator_lib import COLORS, param, series_def, sma, ema

The sidecar adds its own directory to sys.path before loading plugins, so
third-party plugins in a user directory can import this module too.
"""

from __future__ import annotations

import math

# Single source of truth for indicator series colors. Plugins may reference
# names instead of repeating hex strings in every metadata dictionary.
COLORS = {
    "gold": "#f0c75e",
    "blue": "#53b7ff",
    "orange": "#ffb86b",
    "purple": "#c792ea",
    "red": "#f28e8e",
    "teal": "#23b39a",
    "muted": "#9aa4b2",
    "green": "#43d17c",
}

PALETTE = [
    COLORS["gold"],
    COLORS["blue"],
    COLORS["red"],
    COLORS["purple"],
    COLORS["teal"],
    COLORS["orange"],
]


def color(name_or_index):
    """Resolve a color name or palette index to a hex string."""
    if isinstance(name_or_index, int):
        return PALETTE[name_or_index % len(PALETTE)]
    return COLORS.get(str(name_or_index), str(name_or_index))


def param(key, label, default, minimum=0, maximum=1_000_000, kind="int", decimals=0):
    """Build a parameter metadata entry for MANIFEST."""
    result = {
        "key": key,
        "label": label,
        "type": kind,
        "default": default,
        "min": minimum,
        "max": maximum,
    }
    if kind == "float":
        result["decimals"] = decimals
    return result


def series_def(name, label, color_name):
    """Build a series metadata entry for MANIFEST."""
    return {"name": name, "label": label, "color": color(color_name)}


def safe(value):
    """Return a finite float or None."""
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def sma(values, period):
    """Simple moving average, preserving list length with None warm-up."""
    period = max(1, int(period))
    result = [None] * len(values)
    total = 0.0
    valid = 0
    for i, value in enumerate(values):
        if value is None:
            continue
        total += value
        valid += 1
        if i >= period and values[i - period] is not None:
            total -= values[i - period]
            valid -= 1
        if valid == period:
            result[i] = total / period
    return result


def ema(values, period):
    """Exponential moving average; starts from the first valid value."""
    period = max(1, int(period))
    result = [None] * len(values)
    alpha = 2.0 / (period + 1.0)
    previous = None
    for i, value in enumerate(values):
        if value is None:
            continue
        previous = value if previous is None else alpha * value + (1.0 - alpha) * previous
        result[i] = previous
    return result


def stdev(values, period):
    """Population standard deviation over a moving window."""
    period = max(1, int(period))
    result = [None] * len(values)
    for i in range(period - 1, len(values)):
        window = values[i - period + 1 : i + 1]
        if any(value is None for value in window):
            continue
        mean = sum(window) / period
        variance = sum((value - mean) ** 2 for value in window) / period
        result[i] = math.sqrt(variance)
    return result


def boll(values, period=20, mult=2.0):
    """Return (middle, upper, lower) Bollinger bands."""
    middle = sma(values, period)
    deviation = stdev(values, period)
    upper = [None if m is None or d is None else m + mult * d for m, d in zip(middle, deviation)]
    lower = [None if m is None or d is None else m - mult * d for m, d in zip(middle, deviation)]
    return middle, upper, lower


def macd(values, fast=12, slow=26, signal=9):
    """Return (macd, signal, histogram)."""
    fast_line = ema(values, fast)
    slow_line = ema(values, slow)
    macd_line = [None if f is None or s is None else f - s for f, s in zip(fast_line, slow_line)]
    signal_line = ema(macd_line, signal)
    histogram = [None if m is None or s is None else m - s for m, s in zip(macd_line, signal_line)]
    return macd_line, signal_line, histogram


def rsi(values, period=14):
    """Wilder-style RSI, returning None during warm-up."""
    period = max(1, int(period))
    result = [None] * len(values)
    gains = []
    losses = []
    for i in range(1, len(values)):
        previous, current = values[i - 1], values[i]
        if previous is None or current is None:
            gains.append(None)
            losses.append(None)
            continue
        change = current - previous
        gains.append(max(change, 0.0))
        losses.append(max(-change, 0.0))
    if len(gains) < period:
        return result
    avg_gain = sum(g for g in gains[:period] if g is not None) / period
    avg_loss = sum(l for l in losses[:period] if l is not None) / period
    for i in range(period, len(values)):
        gain = gains[i - 1]
        loss = losses[i - 1]
        if gain is None or loss is None:
            continue
        avg_gain = (avg_gain * (period - 1) + gain) / period
        avg_loss = (avg_loss * (period - 1) + loss) / period
        result[i] = 100.0 if avg_loss == 0 else 100.0 - 100.0 / (1.0 + avg_gain / avg_loss)
    return result


def highest(values, period):
    period = max(1, int(period))
    result = [None] * len(values)
    for i in range(period - 1, len(values)):
        window = [v for v in values[i - period + 1 : i + 1] if v is not None]
        if window:
            result[i] = max(window)
    return result


def lowest(values, period):
    period = max(1, int(period))
    result = [None] * len(values)
    for i in range(period - 1, len(values)):
        window = [v for v in values[i - period + 1 : i + 1] if v is not None]
        if window:
            result[i] = min(window)
    return result


def ref(values, offset):
    result = [None] * len(values)
    for i in range(len(values)):
        source = i - int(offset)
        if 0 <= source < len(values):
            result[i] = values[source]
    return result


def crossover(left, right):
    result = [None] * len(left)
    for i in range(1, min(len(left), len(right))):
        if None in (left[i - 1], right[i - 1], left[i], right[i]):
            continue
        result[i] = left[i - 1] <= right[i - 1] and left[i] > right[i]
    return result


def crossunder(left, right):
    result = [None] * len(left)
    for i in range(1, min(len(left), len(right))):
        if None in (left[i - 1], right[i - 1], left[i], right[i]):
            continue
        result[i] = left[i - 1] >= right[i - 1] and left[i] < right[i]
    return result
