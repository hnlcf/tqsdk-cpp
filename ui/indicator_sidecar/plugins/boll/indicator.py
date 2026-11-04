"""BOLL indicator plugin."""

import math

MANIFEST = {
    "id": "boll",
    "name": "BOLL",
    "version": "1.0",
    "panel": "main",
    "params": [
        {"key": "period", "label": "周期", "type": "int", "default": 20, "min": 2, "max": 500},
        {"key": "mult", "label": "倍数", "type": "float", "default": 2.0, "min": 0.1, "max": 10.0,
         "decimals": 2},
    ],
    "series": [
        {"name": "boll_mid", "label": "中轨", "color": "#f0c75e"},
        {"name": "boll_upper", "label": "上轨", "color": "#f28e8e"},
        {"name": "boll_lower", "label": "下轨", "color": "#23b39a"},
    ],
}


def sma(values, period):
    period = max(1, int(period))
    result = [None] * len(values)
    for i in range(period - 1, len(values)):
        window = values[i - period + 1:i + 1]
        if any(value is None for value in window):
            continue
        result[i] = sum(window) / period
    return result


def stdev(values, period):
    period = max(1, int(period))
    result = [None] * len(values)
    for i in range(period - 1, len(values)):
        window = values[i - period + 1:i + 1]
        if any(value is None for value in window):
            continue
        mean = sum(window) / period
        variance = sum((value - mean) ** 2 for value in window) / period
        result[i] = math.sqrt(variance)
    return result


def compute(params, bars):
    closes = [float(bar["close"]) for bar in bars]
    period = max(1, int(params.get("period", 20)))
    mult = max(0.0, float(params.get("mult", 2.0)))
    middle = sma(closes, period)
    deviation = stdev(closes, period)
    upper = [None if m is None or d is None else m + mult * d for m, d in zip(middle, deviation)]
    lower = [None if m is None or d is None else m - mult * d for m, d in zip(middle, deviation)]
    return {"boll_mid": middle, "boll_upper": upper, "boll_lower": lower}
